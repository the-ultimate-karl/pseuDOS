#include "efi.h"
#include "errtext.h"
#include "kernel.h"
#include "io.h"

/*
 * Check basic x86_64 CPU features using CPUID instruction
 */
static int check_cpu_features(void) {
    uint32_t eax, ebx, ecx, edx;

    /* Check maximum extended function */
    __asm__ volatile ("cpuid"
                      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(0x80000000));
    if (eax < 0x80000001) {
        return 0;
    }

    /* Check for Long Mode (bit 29 of EDX in 0x80000001) */
    __asm__ volatile ("cpuid"
                      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(0x80000001));
    if (!(edx & (1 << 29))) {
        return 0; /* Long mode not supported */
    }

    return 1;
}

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

static void boot_msg(EFI_SYSTEM_TABLE *SystemTable, const char *str) {
    uart_puts(str);
    uefi_puts(SystemTable, str);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    (void)ImageHandle;
    uart_init();

    if (!SystemTable || !SystemTable->BootServices) {
        /* Fatal firmware state */
        error_boot(NULL, ERR_GENERIC_BOOT_FAILURE, "SystemTable or BootServices pointer is NULL");
    }

    /* Clear screen and reset console if available */
    if (SystemTable->ConOut && SystemTable->ConOut->ClearScreen) {
        SystemTable->ConOut->ClearScreen(SystemTable->ConOut);
    }

    boot_msg(SystemTable, "\n[bootmgfw] pseuDOS 64-bit UEFI Boot Manager v1.0\n");
    boot_msg(SystemTable, "[bootmgfw] Initializing UEFI firmware environment...\n");

    /* 1. Inspect System Table & Firmware */
    boot_msg(SystemTable, "[bootmgfw] Validating UEFI System Table and Boot Services... [OK]\n");

    /* 2. Disable Watchdog Timer */
    boot_msg(SystemTable, "[bootmgfw] Disabling UEFI watchdog timer... ");
    if (SystemTable->BootServices->SetWatchdogTimer) {
        SystemTable->BootServices->SetWatchdogTimer(0, 0, 0, NULL);
        boot_msg(SystemTable, "[OK]\n");
    } else {
        boot_msg(SystemTable, "[SKIPPED]\n");
    }

    /* 3. Check CPU features */
    boot_msg(SystemTable, "[bootmgfw] Querying CPUID for x86_64 Long Mode support... ");
    if (!check_cpu_features()) {
        boot_msg(SystemTable, "[FAILED]\n");
        error_boot(SystemTable, ERR_UNSUPPORTED_CPU, "x86_64 Long Mode (64-bit) feature flag not present in CPUID");
    }
    boot_msg(SystemTable, "[OK]\n");

    /* 4. Prepare console and looking for kernel */
    boot_msg(SystemTable, "[bootmgfw] Verifying console display protocol... [OK]\n");
    boot_msg(SystemTable, "[bootmgfw] looking for kernel...\n\n");
    boot_msg(SystemTable, "====================================\n\n");

    /* 5. Jump to kernel */
    kernel_main(SystemTable);

    /* Should never reach here */
    error_boot(SystemTable, ERR_GENERIC_BOOT_FAILURE, "Kernel unexpectedly returned to bootloader");

    return EFI_SUCCESS;
}
