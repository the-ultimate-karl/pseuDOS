#include "errtext.h"
#include "io.h"

static void uart_init(void) {
    outb(0x3F8 + 1, 0x00); /* Disable all interrupts */
    outb(0x3F8 + 3, 0x80); /* Enable DLAB (set baud rate divisor) */
    outb(0x3F8 + 0, 0x01); /* Set divisor to 1 (115200 baud) */
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03); /* 8 bits, no parity, one stop bit */
    outb(0x3F8 + 2, 0xC7); /* FIFO control: enable, clear, 14-byte threshold */
    outb(0x3F8 + 4, 0x0B); /* RTS/DSR set */
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
    uart_puts(str);
    uefi_puts(SystemTable, str);
}

void error_boot(EFI_SYSTEM_TABLE *SystemTable, boot_error_t err_code, const char *extra_info) {
    uart_init();

    print_both(SystemTable, "====================================\n\n");
    print_both(SystemTable, "pseuDOS Boot Loader failed to start. A recent change in the structure of\n");
    print_both(SystemTable, "pseuDOS or host hardware configuration might be the culprit.\n\n");

    switch (err_code) {
        case ERR_KERNEL_MOUNT_FAILED:
            print_both(SystemTable, "error: [HARDWARE/VFS] cannot mount root filesystem / kernel image (EFI_NOT_FOUND / EFI_NO_MEDIA)\n\n");
            break;
        case ERR_KERNEL_NOT_FOUND:
            print_both(SystemTable, "error: [STORAGE] cannot find kernel binary (\\KERNEL.BIN or EFI\\BOOT\\KERNEL.ELF): No such file or directory\n\n");
            break;
        case ERR_KERNEL_INTEGRITY:
            print_both(SystemTable, "error: [CHECKSUM] cannot verify integrity of the kernel: invalid magic or corrupted binary\n\n");
            break;
        case ERR_MEMORY_MAP_EXHAUSTED:
            print_both(SystemTable, "error: [MEMORY] physical memory allocation failed or memory map exhausted (EFI_OUT_OF_RESOURCES)\n\n");
            break;
        case ERR_GOP_INIT_FAILED:
            print_both(SystemTable, "error: [DISPLAY] Graphics Output Protocol (GOP) initialization failed / no video mode available\n\n");
            break;
        case ERR_UNSUPPORTED_CPU:
            print_both(SystemTable, "error: [CPU] x86_64 Long Mode, SSE2, or required hardware features unavailable\n\n");
            break;
        case ERR_ACPI_RSDP_NOT_FOUND:
            print_both(SystemTable, "error: [ACPI] Root System Description Pointer (RSDP) not found in UEFI configuration tables\n\n");
            break;
        case ERR_EXIT_BOOT_SERVICES_FAILED:
            print_both(SystemTable, "error: [FIRMWARE] Failed to exit UEFI Boot Services: memory map changed during transition\n\n");
            break;
        case ERR_SECURE_BOOT_VIOLATION:
            print_both(SystemTable, "error: [SECURITY] Kernel image rejected by UEFI Secure Boot policy (EFI_SECURITY_VIOLATION)\n\n");
            break;
        case ERR_STORAGE_DEVICE_IO:
            print_both(SystemTable, "error: [STORAGE_IO] Hard drive / NVMe / SATA block read failure (EFI_DEVICE_ERROR)\n\n");
            break;
        case ERR_GENERIC_BOOT_FAILURE:
        default:
            print_both(SystemTable, "error: unspecified hardware boot failure\n\n");
            break;
    }

    if (extra_info) {
        print_both(SystemTable, "details: ");
        print_both(SystemTable, extra_info);
        print_both(SystemTable, "\n\n");
    }

    print_both(SystemTable, "Please re-check your hardware configuration, boot media, and firmware settings.\n");
    print_both(SystemTable, "System halted!\n\n");
    print_both(SystemTable, "====================================\n\n");

    while (1) {
        __asm__ volatile ("hlt");
    }
}
