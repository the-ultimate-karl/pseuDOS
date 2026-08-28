#include "errtext.h"
#include "io.h"
#include "lib.h"

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

static void uefi_puts(EFI_SYSTEM_TABLE *SystemTable, const char *str) {
    if (!SystemTable || !SystemTable->ConOut || !str) return;
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
}

static void print_both(EFI_SYSTEM_TABLE *SystemTable, const char *str) {
    if (SystemTable && SystemTable->ConOut) {
        uefi_puts(SystemTable, str);
    } else {
        uart_puts(str);
    }
}

static const char *get_error_diagnostic(BootErrorCode code) {
    switch (code) {
        case ERR_KERNEL_MOUNT_FAILED:
            return "[VFS/STORAGE] unable to mount boot device filesystem";
        case ERR_KERNEL_NOT_FOUND:
            return "[STORAGE] cannot find kernel binary (\\KERNEL.BIN or EFI\\BOOT\\KERNEL.ELF): No such file or directory";
        case ERR_KERNEL_INTEGRITY:
            return "[INTEGRITY] kernel executable image corrupted or invalid PE/ELF magic header";
        case ERR_MEMORY_MAP_EXHAUSTED:
            return "[HARDWARE/MEMORY] physical memory allocation failed or memory map exhausted";
        case ERR_GOP_INIT_FAILED:
            return "[HARDWARE/GRAPHICS] failed to initialize Graphics Output Protocol (GOP) display mode";
        case ERR_UNSUPPORTED_CPU:
            return "[HARDWARE/CPU] CPU does not meet minimum architecture requirements (x86_64 Long Mode missing)";
        case ERR_ACPI_RSDP_NOT_FOUND:
            return "[FIRMWARE/ACPI] ACPI RSDP descriptor table not found in UEFI configuration tables";
        case ERR_EXIT_BOOT_SERVICES_FAILED:
            return "[FIRMWARE/SERVICES] failed to exit UEFI Boot Services (memory map key mismatch)";
        case ERR_SECURE_BOOT_VIOLATION:
            return "[SECURITY] UEFI Secure Boot signature verification failed for boot image";
        case ERR_STORAGE_DEVICE_IO:
            return "[HARDWARE/IO] fatal read error encountered from block storage device controller";
        case ERR_GENERIC_BOOT_FAILURE:
        default:
            return "[GENERIC] unhandled fatal bootloader exception";
    }
}

void error_boot(EFI_SYSTEM_TABLE *SystemTable, BootErrorCode code, const char *details) {
    print_both(SystemTable, "\n====================================\n\n");
    print_both(SystemTable, "pseuDOS Boot Loader failed to start. A recent change in the structure of\n");
    print_both(SystemTable, "pseuDOS or host hardware configuration might be the culprit.\n\n");

    print_both(SystemTable, "error: ");
    print_both(SystemTable, get_error_diagnostic(code));
    print_both(SystemTable, "\n\n");

    if (details && details[0] != '\0') {
        print_both(SystemTable, "details: ");
        print_both(SystemTable, details);
        print_both(SystemTable, "\n\n");
    }

    print_both(SystemTable, "Please re-check your hardware configuration, boot media, and firmware settings.\n");
    print_both(SystemTable, "System halted!\n\n");
    print_both(SystemTable, "====================================\n\n");

    /* Halt CPU execution */
    __asm__ volatile ("cli");
    while (1) {
        __asm__ volatile ("hlt");
    }
}
